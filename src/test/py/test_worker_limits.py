from base import pipeline, clean_db

def test_low_max_worker_processes(pipeline, clean_db):
    pipeline.stop()

    try:
        pipeline.run({'max_worker_processes': '2'})
        assert False
    except Exception as e:
        assert 'Background workers failed to start up' in str(e)

    pipeline.stop()
    pipeline.run({'max_worker_processes': '8'})

    pipeline.stop();
    pipeline.run()
